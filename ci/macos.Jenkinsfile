#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.9.43'

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
    ARTIFACT = "pkg/${utils.pkgFilename(name: 'LogosBasecamp', type: 'Desktop', ext: 'dmg', arch: getArch())}"
  }

  stages {
    stage('Smoke Test') {
      steps { script {
        nix.flake('smoke-test-bundle')
      } }
    }

    stage('Build MacOS App Bundle') {
      steps { script {
        nix.flake('bin-macos-app')
      } }
    }

    stage('Sign & Notarize') {
      when {
        expression { utils.isReleaseBuild() }
      }
      steps {
        script {
          logos.codesignApp(
            bundlePath: 'result/LogosBasecamp.app',
            outputPath: env.ARTIFACT,
            mode: 'both',
            timeout: '30m'
          )
        }
      }
    }

    stage('Package') {
      steps {
        sh "./scripts/create-dmg.sh --bundle result/LogosBasecamp.app --output ${env.ARTIFACT}"
      }
    }

    stage('Upload') {
      steps { script {
        env.PKG_URL = s5cmd.upload(env.ARTIFACT)
        jenkins.setBuildDesc(DMG: env.PKG_URL)
      } }
    }

    stage('Archive') {
      steps { script {
        archiveArtifacts(env.ARTIFACT)
      } }
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
