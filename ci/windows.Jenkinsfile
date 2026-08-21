#!/usr/bin/env groovy
library 'status-jenkins-lib@v1.10.3'
def isPRBuild = utils.isPRBuild()

pipeline {
  agent {
    docker {
      label 'linuxcontainer'
      image 'harbor.status.im/infra/ci-build-containers:linux-base-1.0.2'
      args '--volume=/nix:/nix --volume=/etc/nix:/etc/nix'
    }
  }

  parameters {
    booleanParam(
      name: 'RELEASE',
      description: 'Decides whether release credentials are used.',
      defaultValue: params.RELEASE ?: false
    )
    string(
      name: 'BRANCH',
      description: 'Branch to build.',
      defaultValue: 'master'
    )
  }

  options {
    timestamps()
    ansiColor('xterm')
    timeout(time: 45, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
      artifactNumToKeepStr: '1',
    ))
    disableConcurrentBuilds(abortPrevious: isPRBuild)
    copyArtifactPermission('/logos/logos-basecamp/*')
  }

  environment {
    PLATFORM = 'windows/x86_64'
    ARTIFACT = "pkg/${utils.pkgFilename(name: 'LogosBasecamp', type: 'Desktop', ext: 'zip', arch: 'x86_64')}"
  }

  stages {
    stage('Build') {
      steps { script { nix.flake('packages.x86_64-windows.bin-bundle-dir') } }
    }

    stage('Package') {
      steps { sh "./scripts/create-exe.sh --bundle result --output ${env.ARTIFACT}" }
    }

    stage('Upload') {
      steps { script {
        env.PKG_URL = s5cmd.upload(env.ARTIFACT)
        jenkins.setBuildDesc(Windows: env.PKG_URL)
      } }
    }

    stage('Archive') {
      steps { archiveArtifacts(env.ARTIFACT) }
    }

    stage('Sign') {
      when { expression { params.RELEASE } }
      steps { script {
        build(
          job: 'logos/logos-basecamp/systems/windows/x86_64/sign',
          wait: true,
          parameters: [
            string(name: 'BUILD_JOB', value: env.JOB_NAME),
            string(name: 'BUILD_NUMBER', value: env.BUILD_NUMBER),
          ]
        )
      } }
    }
  }

  post {
    success { script {
      github.notifyPR(true)
      if (params.RELEASE) {
        github.statusUpdate(context: "jenkins/${params.BRANCH}", state: 'SUCCESS', message: 'Release pipeline passed')
      }
    } }
    failure { script {
      github.notifyPR(false)
      if (params.RELEASE) {
        github.statusUpdate(context: "jenkins/${params.BRANCH}", state: 'FAILURE', message: "Release pipeline ${currentBuild.currentResult}")
      }
    } }
    cleanup {
      cleanWs(disableDeferredWipeout: true)
      dir(env.WORKSPACE_TMP) { deleteDir() }
    }
  }
}