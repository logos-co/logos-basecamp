#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.9.41'

def isPRBuild = utils.isPRBuild()

pipeline {
  agent {
    docker {
      label 'linuxcontainer'
      image 'harbor.status.im/infra/ci-build-containers:linux-base-1.0.0'
      args '--volume=/nix:/nix ' +
           '--volume=/etc/nix:/etc/nix '
    }
  }

  parameters {
    booleanParam(
      name: 'RELEASE',
      description: 'Decides whether release credentials are used.',
      defaultValue: params.RELEASE ?: false
    )
  }

  options {
    timestamps()
    ansiColor('xterm')
    timeout(time: 30, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
      artifactNumToKeepStr: '1',
    ))
    disableConcurrentBuilds(
      abortPrevious: isPRBuild
    )
    /* Allows combined build to copy */
    copyArtifactPermission('/logos-basecamp/*')
  }

  environment {
    PLATFORM = "linux/${getArch()}"
    ARTIFACT = "pkg/${utils.pkgFilename(name: 'LogosBasecamp', type: 'Desktop', ext: 'AppImage', arch: getArch())}"
  }

  stages {
    stage('Smoke Test') {
      steps { script {
        nix.flake('smoke-test')
      } }
    }

    stage('Build AppImage') {
      steps { script {
        nix.flake("bin-appimage")
      } }
    }

    stage('Package') {
      steps {
        sh 'mkdir -p pkg'
        sh "cp result/logos-basecamp.AppImage '${env.ARTIFACT}'"
      }
    }

    stage('Upload') {
      steps { script {
        env.PKG_URL = s5cmd.upload(env.ARTIFACT)
        jenkins.setBuildDesc(AppImage: env.PKG_URL)
      } }
    }

    stage('Archive') {
      steps {
        archiveArtifacts(env.ARTIFACT)
      }
    }
  }

  post {
    success { script { github.notifyPR(true) } }
    failure { script { github.notifyPR(false) } }
    cleanup {
      cleanWs(disableDeferredWipeout: true)
      dir(env.WORKSPACE_TMP) { deleteDir() }
    }
  }
}

def getArch() {
  def tokens = Thread.currentThread().getName().split('/')
  for (def arch in ['x86_64', 'aarch64']) {
    if (tokens.contains(arch)) { return arch }
  }
}
