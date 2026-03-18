#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.9.41'

def isPRBuild = utils.isPRBuild()

pipeline {
  agent { label "macos && ${getArch()} && nix-2.24" }

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
    PLATFORM = "macos/${getArch()}"
    ARTIFACT = "pkg/${utils.pkgFilename(name: 'LogosBasecamp', ext: 'dmg', arch: getArch())}"
  }

  stages {
    stage('Build DMG') {
      steps { script {
        nix.flake("dmg")
      } }
    }

    stage('Smoke Test') {
      steps {
        sh 'nix build .#smoke-test-bundle --out-link result-smoke -L --extra-experimental-features "nix-command flakes"'
        sh 'cat result-smoke/smoke-test.log'
      }
    }

    stage('Package') {
      steps {
        sh 'mkdir -p pkg'
        sh "cp result/LogosBasecamp-*.dmg '${env.ARTIFACT}'"
      }
    }

    stage('Upload') {
      steps { script {
        env.PKG_URL = s5cmd.upload(env.ARTIFACT)
        jenkins.setBuildDesc(DMG: env.PKG_URL)
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
